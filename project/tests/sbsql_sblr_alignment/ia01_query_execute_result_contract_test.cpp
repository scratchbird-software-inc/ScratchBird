// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/query_execute_result_handle.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <type_traits>

namespace sblr = scratchbird::engine::sblr;

namespace {

using Field = sblr::QueryExecuteResultHandleFieldV1;
using Uuid = scratchbird::core::platform::Uuid;
static_assert(sizeof(Uuid) == 16);
static_assert(std::is_same_v<decltype(Field::value), Uuid>);
static_assert(!std::is_constructible_v<Uuid, std::string>);

Uuid Identity(std::uint8_t id) {
  return Uuid{{0x01, 0x9c, 0x12, 0x34, 0, id, 0x70, 0,
               0x80, 0, 0, 0, 0, 0, 0, id}};
}

std::vector<Field> ValidFields() {
  return {
      {"execution_uuid", "desc.uuid",
       Identity(1)},
      {"result_set_uuid", "desc.uuid",
       Identity(2)},
      {"row_descriptor_uuid", "desc.uuid",
       Identity(3)},
      {"snapshot_uuid", "desc.uuid",
       Identity(4)},
  };
}

bool Refused(std::string_view shape, std::uint32_t version,
             const std::vector<Field>& fields, std::string_view detail) {
  const auto result =
      sblr::ValidateQueryExecuteResultHandleV1(shape, version, fields);
  return !result.ok && result.diagnostic_id == "DATATYPE.DESCRIPTOR.INVALID" &&
         result.detail == detail && result.handle.execution_uuid.is_nil() &&
         result.handle.result_set_uuid.is_nil() &&
         result.handle.row_descriptor_uuid.is_nil() &&
         result.handle.snapshot_uuid.is_nil();
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
  nil[3].value = {};
  if (!Refused("query_execute_result", 1, nil,
               "query_execute_result_field_contract_invalid")) {
    return 6;
  }
  // Text spelling is not representable in this engine API. User UUID data
  // versions are separate: all four identities here must be system UUIDv7.
  std::size_t checked = 0;
  for (std::size_t role = 0; role < fields.size(); ++role) {
    for (std::size_t byte = 0; byte < 16; ++byte) {
      for (unsigned value = 0; value < 256; ++value) {
        auto candidate = fields;
        candidate[role].value.bytes[byte] = static_cast<std::uint8_t>(value);
        const bool expected =
            (byte != 6 || (value >> 4) == 7) &&
            (byte != 8 || (value >> 6) == 2);
        const auto actual = sblr::ValidateQueryExecuteResultHandleV1(
            "query_execute_result", 1, candidate);
        if (actual.ok != expected) return 7;
        if (expected) {
          if (actual.handle.execution_uuid != candidate[0].value ||
              actual.handle.result_set_uuid != candidate[1].value ||
              actual.handle.row_descriptor_uuid != candidate[2].value ||
              actual.handle.snapshot_uuid != candidate[3].value) return 10;
        } else if (!Refused("query_execute_result", 1, candidate,
                            "query_execute_result_field_contract_invalid")) {
          return 11;
        }
        ++checked;
      }
    }
    auto missing_name = fields;
    missing_name[role].name.clear();
    if (!Refused("query_execute_result", 1, missing_name,
                 "query_execute_result_field_contract_invalid")) return 12;
    for (std::size_t other = role + 1; other < fields.size(); ++other) {
      auto same_identity = fields;
      same_identity[other].value = same_identity[role].value;
      if (!Refused("query_execute_result", 1, same_identity,
                   "query_execute_result_identity_roles_duplicated")) return 13;
    }
  }
  auto duplicate = fields;
  duplicate[3].value = duplicate[0].value;
  if (!Refused("query_execute_result", 1, duplicate,
               "query_execute_result_identity_roles_duplicated")) {
    return 8;
  }

  // No synthetic row fixture is execution evidence for this shape validator.
  auto extra = fields;
  extra.push_back(fields.front());
  if (!Refused("query_execute_result", 1, extra,
               "query_execute_result_exact_cardinality_invalid")) return 14;
  std::cout << "PASS binary query handle mutations=" << checked << '\n';
  return 0;
}
