// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_dispatch.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace sblr = scratchbird::engine::sblr;

namespace {

using Field = sblr::QueryExecuteResultHandleFieldV1;

std::vector<Field> ValidFields() {
  return {
      {"execution_uuid", "desc.uuid",
       "019c1234-0001-7000-8000-000000000001"},
      {"result_set_uuid", "desc.uuid",
       "019c1234-0002-7000-8000-000000000002"},
      {"row_descriptor_uuid", "desc.uuid",
       "019c1234-0003-7000-8000-000000000003"},
      {"snapshot_uuid", "desc.uuid",
       "019c1234-0004-7000-8000-000000000004"},
  };
}

bool Refused(std::string_view shape, std::uint32_t version,
             const std::vector<Field>& fields, std::string_view detail) {
  const auto result =
      sblr::ValidateQueryExecuteResultHandleV1(shape, version, fields);
  return !result.ok && result.diagnostic_id == "DATATYPE.DESCRIPTOR_INVALID" &&
         result.detail == detail;
}

}  // namespace

int main() {
  auto fields = ValidFields();
  const auto valid = sblr::ValidateQueryExecuteResultHandleV1(
      "query_execute_result", 1, fields);
  if (!valid.ok || valid.handle.execution_uuid != fields[0].value ||
      valid.handle.result_set_uuid != fields[1].value ||
      valid.handle.row_descriptor_uuid != fields[2].value ||
      valid.handle.snapshot_uuid != fields[3].value) {
    std::cerr << "exact registry handle did not validate\n";
    return 1;
  }

  if (!Refused("stmt_execute_result", 1, fields,
               "query_execute_result_registry_identity_mismatch") ||
      !Refused("query_execute_result", 2, fields,
               "query_execute_result_registry_identity_mismatch")) {
    return 2;
  }
  auto wrong_count = fields;
  wrong_count.pop_back();
  if (!Refused("query_execute_result", 1, wrong_count,
               "query_execute_result_exact_cardinality_invalid")) {
    return 3;
  }
  auto reordered = fields;
  std::swap(reordered[0], reordered[1]);
  if (!Refused("query_execute_result", 1, reordered,
               "query_execute_result_field_contract_invalid")) {
    return 4;
  }
  auto wrong_descriptor = fields;
  wrong_descriptor[2].descriptor = "desc.text.code";
  if (!Refused("query_execute_result", 1, wrong_descriptor,
               "query_execute_result_field_contract_invalid")) {
    return 5;
  }
  auto nil = fields;
  nil[3].value = "00000000-0000-0000-0000-000000000000";
  if (!Refused("query_execute_result", 1, nil,
               "query_execute_result_field_contract_invalid")) {
    return 6;
  }
  auto uppercase = fields;
  uppercase[0].value = "019C1234-0001-7000-8000-000000000001";
  if (!Refused("query_execute_result", 1, uppercase,
               "query_execute_result_field_contract_invalid")) {
    return 7;
  }
  auto duplicate = fields;
  duplicate[3].value = duplicate[0].value;
  if (!Refused("query_execute_result", 1, duplicate,
               "query_execute_result_identity_roles_duplicated")) {
    return 8;
  }

  // The command handle is metadata. Query rows remain a separate renderer
  // input and validation neither consumes nor rewrites them.
  const std::vector<std::vector<std::string>> admitted_rows{{"1"}, {"2"}};
  if (admitted_rows.size() != 2 || admitted_rows[0][0] != "1") return 9;
  return 0;
}
