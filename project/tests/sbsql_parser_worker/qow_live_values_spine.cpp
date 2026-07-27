// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

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

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.statement_uuid.canonical =
      "019f0000-0000-7120-8000-000000008101";
  context.local_transaction_id = 8101;
  context.snapshot_visible_through_local_transaction_id = 8099;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_uuid.canonical = kCatalogEpochUuid;
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
  return envelope;
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
       "019f0000-0000-7400-8000-000000008312|1|-|-|-|-|-"},
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
  return envelope;
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
  return envelope;
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
  return envelope;
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
  return envelope;
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
  return envelope;
}

struct PairStatisticalAggregateProfile {
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
        {"CORR",
         "qow.live.values.corr-expression",
         "aggregate.global-corr-expression.v1",
         "a0",
         "019dffbb-f000-77bb-ba9b-2e78acf84521",
         "corr_value",
         1.0,
         false},
        {"COVAR_POP",
         "qow.live.values.covar-pop-expression",
         "aggregate.global-covar-pop-expression.v1",
         "a1",
         "019dffbb-f000-7f09-8ceb-17ad4c70e99f",
         "covar_pop_value",
         4.0 / 3.0,
         false},
        {"COVAR_SAMP",
         "qow.live.values.covar-samp-expression",
         "aggregate.global-covar-samp-expression.v1",
         "a2",
         "019dffbb-f000-747d-bc01-caad9137d070",
         "covar_samp_value",
         2.0,
         false},
        {"REGR_COUNT",
         "qow.live.values.regr-count-expression",
         "aggregate.global-regr-count-expression.v1",
         "a3",
         "019dffbb-f000-75aa-bbe6-a4a67dacb81f",
         "regr_count_value",
         3.0,
         true},
        {"REGR_AVGX",
         "qow.live.values.regr-avgx-expression",
         "aggregate.global-regr-avgx-expression.v1",
         "a4",
         "019dffbb-f000-7662-a816-d1df50e9b664",
         "regr_avgx_value",
         2.0,
         false},
        {"REGR_AVGY",
         "qow.live.values.regr-avgy-expression",
         "aggregate.global-regr-avgy-expression.v1",
         "a5",
         "019dffbb-f000-7d03-ac2d-753cdb7744c0",
         "regr_avgy_value",
         4.0,
         false},
        {"REGR_INTERCEPT",
         "qow.live.values.regr-intercept-expression",
         "aggregate.global-regr-intercept-expression.v1",
         "a6",
         "019dffbb-f000-7c7c-b576-d67ea9d4bcbb",
         "regr_intercept_value",
         0.0,
         false},
        {"REGR_R2",
         "qow.live.values.regr-r2-expression",
         "aggregate.global-regr-r2-expression.v1",
         "a7",
         "019dffbb-f000-7a43-9a28-a119b31d9c20",
         "regr_r2_value",
         1.0,
         false},
        {"REGR_SLOPE",
         "qow.live.values.regr-slope-expression",
         "aggregate.global-regr-slope-expression.v1",
         "a8",
         "019dffbb-f000-7f80-b81a-5240a6dbab55",
         "regr_slope_value",
         2.0,
         false},
        {"REGR_SXX",
         "qow.live.values.regr-sxx-expression",
         "aggregate.global-regr-sxx-expression.v1",
         "a9",
         "019dffbb-f000-735e-9e55-5f9243786403",
         "regr_sxx_value",
         2.0,
         false},
        {"REGR_SXY",
         "qow.live.values.regr-sxy-expression",
         "aggregate.global-regr-sxy-expression.v1",
         "aa",
         "019dffbb-f000-788b-a249-866547a43ebe",
         "regr_sxy_value",
         4.0,
         false},
        {"REGR_SYY",
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
  return envelope;
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
  return envelope;
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
  return envelope;
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
  return envelope;
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
  return envelope;
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
  return envelope;
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
  return envelope;
}

bool ValidateLiveValuesSpine() {
  const auto first =
      sblr::DispatchSblrOperation({Context(), ValuesEnvelope(), {}});
  const auto repeated =
      sblr::DispatchSblrOperation({Context(), ValuesEnvelope(), {}});
  bool passed = true;
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

bool ValidateComposedScalarValuesSpine() {
  const auto result = sblr::DispatchSblrOperation(
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
  const auto first = sblr::DispatchSblrOperation(
      {Context(), UnionAllValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchSblrOperation(
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
  const auto result = sblr::DispatchSblrOperation(
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

bool ValidateInnerJoinValuesSpine() {
  const auto first = sblr::DispatchSblrOperation(
      {Context(), InnerJoinValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchSblrOperation(
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
  const auto false_result = sblr::DispatchSblrOperation(
      {Context(), std::move(false_predicate), {}});
  const auto unknown_result = sblr::DispatchSblrOperation(
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
    const auto result = sblr::DispatchSblrOperation(
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

bool ValidateFilterValuesSpine() {
  const auto first = sblr::DispatchSblrOperation(
      {Context(), FilterValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchSblrOperation(
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
  const auto false_result = sblr::DispatchSblrOperation(
      {Context(), std::move(false_predicate), {}});
  const auto unknown_result = sblr::DispatchSblrOperation(
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
    const auto result = sblr::DispatchSblrOperation(
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
  const auto first = sblr::DispatchSblrOperation(
      {Context(), ProjectValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchSblrOperation(
      {Context(), ProjectValuesEnvelope(), {}});
  auto dropped_envelope = ProjectValuesEnvelope();
  for (auto& operand : dropped_envelope.operands) {
    if (operand.type == "relational_node_v1" && operand.name == "2") {
      operand.value = "3|0|1|2|-";
    }
  }
  const auto dropped = sblr::DispatchSblrOperation(
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
    const auto result = sblr::DispatchSblrOperation(
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

bool ValidateLimitValuesSpine() {
  const auto first = sblr::DispatchSblrOperation(
      {Context(), LimitValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchSblrOperation(
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
  const auto zero = sblr::DispatchSblrOperation(
      {Context(), std::move(zero_envelope), {}});
  const auto oversized = sblr::DispatchSblrOperation(
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
    const auto result = sblr::DispatchSblrOperation(
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

bool ValidateGlobalCountStarValuesSpine() {
  const auto first = sblr::DispatchSblrOperation(
      {Context(), GlobalCountStarValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchSblrOperation(
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
    const auto result = sblr::DispatchSblrOperation(
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
  const auto first = sblr::DispatchSblrOperation(
      {Context(), GlobalCountExpressionValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchSblrOperation(
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
    const auto result = sblr::DispatchSblrOperation(
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
  const auto first = sblr::DispatchSblrOperation(
      {Context(), GlobalSumExpressionValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchSblrOperation(
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
    const auto result = sblr::DispatchSblrOperation(
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

bool ValidateGlobalAvgExpressionValuesSpine() {
  const auto first = sblr::DispatchSblrOperation(
      {Context(), GlobalAvgExpressionValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchSblrOperation(
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
    const auto result = sblr::DispatchSblrOperation(
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
  const auto first = sblr::DispatchSblrOperation(
      {Context(), GlobalExtremumExpressionValuesEnvelope(maximum), {}});
  const auto repeated = sblr::DispatchSblrOperation(
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
    const auto result = sblr::DispatchSblrOperation(
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
  const auto first = sblr::DispatchSblrOperation(
      {Context(), GlobalBooleanAggregateExpressionValuesEnvelope(kind), {}});
  const auto repeated = sblr::DispatchSblrOperation(
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
    const auto result = sblr::DispatchSblrOperation(
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
    const auto first = sblr::DispatchSblrOperation(
        {Context(),
         GlobalStatisticalAggregateExpressionValuesEnvelope(profile), {}});
    const auto repeated = sblr::DispatchSblrOperation(
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
      const auto result = sblr::DispatchSblrOperation(
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

bool ValidateGlobalPairStatisticalAggregateExpressionValuesSpine() {
  bool passed = true;
  for (const auto& profile : kPairStatisticalAggregateProfiles) {
    const auto first = sblr::DispatchSblrOperation(
        {Context(),
         GlobalPairStatisticalAggregateExpressionValuesEnvelope(profile), {}});
    const auto repeated = sblr::DispatchSblrOperation(
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
      const auto result = sblr::DispatchSblrOperation(
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
    const auto first = sblr::DispatchSblrOperation(
        {Context(), GlobalOrderedSetAggregateValuesEnvelope(profile), {}});
    const auto repeated = sblr::DispatchSblrOperation(
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
        sblr::DispatchSblrOperation({Context(), std::move(envelope), {}});
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

bool ValidateGlobalStringAggExpressionValuesSpine() {
  const auto first = sblr::DispatchSblrOperation(
      {Context(), GlobalStringAggExpressionValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchSblrOperation(
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
    const auto result = sblr::DispatchSblrOperation(
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
  const auto first = sblr::DispatchSblrOperation(
      {Context(), OrderedStringAggExpressionValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchSblrOperation(
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
    const auto result = sblr::DispatchSblrOperation(
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
    const auto first = sblr::DispatchSblrOperation(
        {Context(), OrderedListaggExpressionValuesEnvelope(test_case.profile),
         {}});
    const auto repeated = sblr::DispatchSblrOperation(
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

  const auto overflow_error = sblr::DispatchSblrOperation(
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
    const auto result = sblr::DispatchSblrOperation(
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

bool ValidateOrderedSingleCollectionValuesSpine() {
  bool passed = true;
  for (const auto& profile : kOrderedSingleCollectionProfiles) {
    const auto first = sblr::DispatchSblrOperation(
        {Context(), OrderedSingleCollectionValuesEnvelope(profile), {}});
    const auto repeated = sblr::DispatchSblrOperation(
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
      const auto result = sblr::DispatchSblrOperation(
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

bool ValidateOrderedJsonObjectAggValuesSpine() {
  const auto first = sblr::DispatchSblrOperation(
      {Context(), OrderedJsonObjectAggValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchSblrOperation(
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
    const auto result = sblr::DispatchSblrOperation(
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

bool ValidateSortValuesSpine() {
  const auto first = sblr::DispatchSblrOperation(
      {Context(), SortValuesEnvelope(), {}});
  const auto repeated = sblr::DispatchSblrOperation(
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
    const auto result = sblr::DispatchSblrOperation(
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

bool ValidatePayloadRefusalIsAtomic() {
  auto malformed = ValuesEnvelope();
  malformed.operands[7].value = "1|-|1|-|-|1|-|6e6f74";
  auto invalid_uuid = ValuesEnvelope();
  invalid_uuid.operands[7].value = "1|-|1|-|-|5|-|6e6f74";
  auto ambiguous_temporal = ValuesEnvelope();
  ambiguous_temporal.operands[7].value =
      "1|-|1|-|-|4|-|323032362d30372d32365430303a30303a30305a";
  const auto refused_atomically = [](sblr::SblrOperationEnvelope envelope) {
    const auto result = sblr::DispatchSblrOperation(
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
          refused_atomically(std::move(ambiguous_temporal)),
      "invalid or type-ambiguous VALUES payload published partial evidence");
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
    const auto result = sblr::DispatchSblrOperation(
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
                      ValidateComposedScalarValuesSpine() &&
                      ValidateUnionAllValuesSpine() &&
                      ValidateUnionAllRefusalIsAtomic() &&
                      ValidateInnerJoinValuesSpine() &&
                      ValidateInnerJoinThreeValuedPredicate() &&
                      ValidateInnerJoinRefusalIsAtomic() &&
                      ValidateFilterValuesSpine() &&
                      ValidateFilterThreeValuedPredicate() &&
                      ValidateFilterRefusalIsAtomic() &&
                      ValidateProjectValuesSpine() &&
                      ValidateProjectRefusalIsAtomic() &&
                      ValidateLimitValuesSpine() &&
                      ValidateLimitRefusalIsAtomic() &&
                      ValidateGlobalCountStarValuesSpine() &&
                      ValidateGlobalCountStarRefusalIsAtomic() &&
                      ValidateGlobalCountExpressionValuesSpine() &&
                      ValidateGlobalCountExpressionRefusalIsAtomic() &&
                      ValidateGlobalSumExpressionValuesSpine() &&
                      ValidateGlobalSumExpressionRefusalIsAtomic() &&
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
                      ValidateGlobalPairStatisticalAggregateExpressionValuesSpine() &&
                      ValidateGlobalPairStatisticalAggregateExpressionRefusalIsAtomic() &&
                      ValidateGlobalOrderedSetAggregateValuesSpine() &&
                      ValidateGlobalOrderedSetAggregateRefusalIsAtomic() &&
                      ValidateGlobalStringAggExpressionValuesSpine() &&
                      ValidateGlobalStringAggExpressionRefusalIsAtomic() &&
                      ValidateOrderedStringAggExpressionValuesSpine() &&
                      ValidateOrderedStringAggExpressionRefusalIsAtomic() &&
                      ValidateOrderedListaggExpressionValuesSpine() &&
                      ValidateOrderedListaggExpressionRefusalIsAtomic() &&
                      ValidateOrderedSingleCollectionValuesSpine() &&
                      ValidateOrderedSingleCollectionRefusalIsAtomic() &&
                      ValidateOrderedJsonObjectAggValuesSpine() &&
                      ValidateOrderedJsonObjectAggRefusalIsAtomic() &&
                      ValidateSortValuesSpine() &&
                      ValidateSortRefusalIsAtomic() &&
                      ValidatePayloadRefusalIsAtomic() &&
                      ValidateComposedScalarRefusalIsAtomic();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
