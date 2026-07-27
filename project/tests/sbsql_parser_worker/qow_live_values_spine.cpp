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

bool ValidatePayloadRefusalIsAtomic() {
  auto malformed = ValuesEnvelope();
  malformed.operands[7].value = "1|-|1|-|-|1|-|6e6f74";
  const auto result =
      sblr::DispatchSblrOperation({Context(), std::move(malformed), {}});
  return Require(
      result.accepted && result.optimizer_admitted &&
          !result.optimizer_selected && !result.physical_dag_published &&
          !result.physical_dag_executed &&
          !result.runtime_actuals_attached &&
          !result.canonical_result_published && !result.api_result.ok &&
          result.physical_node_count == 0 &&
          result.canonical_result_bytes.empty() &&
          HasApiDiagnostic(
              result, "QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1"),
      "malformed later VALUES payload published partial plan/result evidence");
}

}  // namespace

// QOW-TEST-INTEGRATION-306-211-LIVE-VALUES-V1
int main() {
  const bool passed =
      ValidateLiveValuesSpine() && ValidatePayloadRefusalIsAtomic();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
