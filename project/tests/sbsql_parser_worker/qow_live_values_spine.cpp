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
                      ValidateSortValuesSpine() &&
                      ValidateSortRefusalIsAtomic() &&
                      ValidatePayloadRefusalIsAtomic() &&
                      ValidateComposedScalarRefusalIsAtomic();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
