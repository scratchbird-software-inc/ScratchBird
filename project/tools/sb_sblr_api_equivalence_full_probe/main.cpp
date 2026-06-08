// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbsql_v3_api_mapping_catalog.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace mapping = scratchbird::parser::sbsql_v3_api_mapping;

namespace {

std::string JsonEscape(std::string_view text) {
  std::string out;
  for (char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default: out += ch; break;
    }
  }
  return out;
}

void Expect(bool condition, std::string message, std::vector<std::string>* errors) {
  if (!condition) errors->push_back(std::move(message));
}

}  // namespace

int main() {
  std::vector<std::string> errors;
  const mapping::ApiMappingEntry select_entry{
      .sblr_operation = "SBLR_SBSQL_V3_SELECT",
      .api_operation_id = "dml.select_rows",
      .api_function_name = "EngineSelectRows",
      .request_type = "EngineSelectRowsRequest",
      .result_type = "EngineSelectRowsResult",
      .payload_class = "query_tree_payload",
      .mapping_status = "existing_engine_api_contract",
      .typed_request_mapping = true,
      .raw_sql_fallback_allowed = false,
      .cluster_authority_required = false,
      .fail_closed_without_cluster_authority = false,
  };
  const mapping::SblrToApiRequestInput select_input{
      .sblr_operation = "SBLR_SBSQL_V3_SELECT",
      .bound_root_uuid = "018f0000-0000-7000-8000-000000000123",
      .descriptor_digest = "descriptor-digest-1",
      .contains_raw_sql_text = false,
      .cluster_authority_present = false,
  };
  const auto select_request = mapping::MapSblrToApiRequest(select_entry, select_input);
  Expect(select_request.ok, "existing select SBLR mapping should succeed", &errors);
  Expect(select_request.api_operation_id == "dml.select_rows", "existing select mapping should preserve direct API operation ID", &errors);
  Expect(select_request.request_type == "EngineSelectRowsRequest", "existing select mapping should preserve request type", &errors);

  auto raw_sql = select_input;
  raw_sql.contains_raw_sql_text = true;
  const auto raw_result = mapping::MapSblrToApiRequest(select_entry, raw_sql);
  Expect(!raw_result.ok, "raw SQL fallback should be rejected", &errors);
  Expect(raw_result.diagnostic_code == "SBSQL_V3_RAW_SQL_FALLBACK_FORBIDDEN", "raw SQL diagnostic mismatch", &errors);

  const mapping::ApiMappingEntry generated_entry{
      .sblr_operation = "SBLR_SBSQL_V3_SHOW_CAPABILITIES",
      .api_operation_id = "sbsql_v3.show_capabilities",
      .api_function_name = "EngineSbsqlV3ShowCapabilities",
      .request_type = "EngineSbsqlV3ShowCapabilitiesRequest",
      .result_type = "EngineSbsqlV3ShowCapabilitiesResult",
      .payload_class = "inspection_payload",
      .mapping_status = "stage_5_contract_defined_behavior_pending",
      .typed_request_mapping = true,
      .raw_sql_fallback_allowed = false,
      .cluster_authority_required = false,
      .fail_closed_without_cluster_authority = false,
  };
  auto generated_input = select_input;
  generated_input.sblr_operation = generated_entry.sblr_operation;
  const auto generated_result = mapping::MapSblrToApiRequest(generated_entry, generated_input);
  Expect(generated_result.ok, "generated pending API contract should still map to a typed request", &errors);
  Expect(generated_result.request_type == "EngineSbsqlV3ShowCapabilitiesRequest", "generated request type mismatch", &errors);

  const mapping::ApiMappingEntry cluster_entry{
      .sblr_operation = "SBLR_SBSQL_V3_SHOW_CLUSTER_STATE",
      .api_operation_id = "cluster.inspect_state",
      .api_function_name = "EngineInspectClusterState",
      .request_type = "EngineInspectClusterStateRequest",
      .result_type = "EngineInspectClusterStateResult",
      .payload_class = "cluster_placeholder_payload",
      .mapping_status = "existing_engine_api_contract",
      .typed_request_mapping = true,
      .raw_sql_fallback_allowed = false,
      .cluster_authority_required = true,
      .fail_closed_without_cluster_authority = true,
  };
  auto cluster_input = select_input;
  cluster_input.sblr_operation = cluster_entry.sblr_operation;
  cluster_input.cluster_authority_present = false;
  const auto cluster_result = mapping::MapSblrToApiRequest(cluster_entry, cluster_input);
  Expect(!cluster_result.ok, "cluster mapping without authority should fail closed", &errors);
  Expect(cluster_result.diagnostic_code == "SBSQL_V3_CLUSTER_AUTHORITY_REQUIRED", "cluster authority diagnostic mismatch", &errors);

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n";
  std::cout << "  \"existing_api_operation_id\": \"" << JsonEscape(select_request.api_operation_id) << "\",\n";
  std::cout << "  \"generated_api_operation_id\": \"" << JsonEscape(generated_result.api_operation_id) << "\",\n";
  std::cout << "  \"errors\": [";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i != 0) std::cout << ", ";
    std::cout << '"' << JsonEscape(errors[i]) << '"';
  }
  std::cout << "]\n}\n";
  return errors.empty() ? 0 : 1;
}
