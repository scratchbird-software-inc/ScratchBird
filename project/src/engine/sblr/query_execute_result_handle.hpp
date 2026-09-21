// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "uuid.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {

struct QueryExecuteResultHandleFieldV1 {
  std::string name;
  std::string descriptor;
  core::platform::Uuid value;
};

struct QueryExecuteResultHandleV1 {
  core::platform::Uuid execution_uuid;
  core::platform::Uuid result_set_uuid;
  core::platform::Uuid row_descriptor_uuid;
  core::platform::Uuid snapshot_uuid;
};

struct QueryExecuteResultHandleValidationV1 {
  bool ok = false;
  QueryExecuteResultHandleV1 handle;
  std::string diagnostic_id;
  std::string detail;
};

// Shape validation only: the caller retains ownership of the real execution,
// result, descriptor and snapshot. A valid identity cannot create that authority.
inline QueryExecuteResultHandleValidationV1 ValidateQueryExecuteResultHandleV1(
    std::string_view result_shape_id, std::uint32_t result_shape_version,
    const std::vector<QueryExecuteResultHandleFieldV1>& fields) {
  QueryExecuteResultHandleValidationV1 result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic_id = "DATATYPE.DESCRIPTOR.INVALID";
    result.detail = std::move(detail);
    return result;
  };
  if (result_shape_id != "query_execute_result" || result_shape_version != 1)
    return refuse("query_execute_result_registry_identity_mismatch");
  constexpr std::array<std::string_view, 4> names{
      "execution_uuid", "result_set_uuid", "row_descriptor_uuid", "snapshot_uuid"};
  if (fields.size() != names.size())
    return refuse("query_execute_result_exact_cardinality_invalid");
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (fields[index].name != names[index] ||
        fields[index].descriptor != "desc.uuid" ||
        !core::uuid::IsEngineIdentityUuid(fields[index].value))
      return refuse("query_execute_result_field_contract_invalid");
  }
  for (std::size_t left = 0; left < fields.size(); ++left)
    for (std::size_t right = left + 1; right < fields.size(); ++right)
      if (fields[left].value == fields[right].value)
        return refuse("query_execute_result_identity_roles_duplicated");
  result.handle = {fields[0].value, fields[1].value,
                   fields[2].value, fields[3].value};
  result.ok = true;
  return result;
}

}  // namespace scratchbird::engine::sblr
